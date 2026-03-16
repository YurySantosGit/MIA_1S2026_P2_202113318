import { Injectable } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { Observable } from 'rxjs';

export interface HealthResponse {
  status: string;
}

export interface ReportItem {
  name: string;
  path: string;
}

export interface ExecuteResponse {
  success: boolean;
  output: string;
  reports: ReportItem[];
}

@Injectable({
  providedIn: 'root'
})
export class BackendService {
  private apiUrl = 'http://localhost:8080/api';

  constructor(private http: HttpClient) {}

  checkHealth(): Observable<HealthResponse> {
    return this.http.get<HealthResponse>(`${this.apiUrl}/health`);
  }

  executeCommands(commands: string): Observable<ExecuteResponse> {
    return this.http.post<ExecuteResponse>(`${this.apiUrl}/execute`, {
      commands
    });
  }
}